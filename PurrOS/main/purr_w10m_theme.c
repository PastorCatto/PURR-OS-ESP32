/*
 * purr_w10m_theme.c - the theme (purr_w10m_theme.h, plan7.md).
 */
#include "purr_brand.h"
#include "purr_w10m_theme.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "themes/lv_theme_private.h"

#include "commands.h"
#include "purr_fs.h"
#include "purr_settings.h"
#include "purr_ui_service.h"

static const char *TAG = "w10m-theme";

static const purr_accent_t s_accents[] = {
    {"Lime", 0xA4C400}, {"Green", 0x60A917}, {"Emerald", 0x00BA00}, {"Teal", 0x00ABA9},
    {"Cyan", 0x1BA1E2}, {"Cobalt", 0x0050EF}, {"Indigo", 0x6A00FF}, {"Violet", 0xAA00FF},
    {"Pink", 0xF472D0}, {"Magenta", 0xD80073}, {"Crimson", 0xA20025}, {"Red", 0xE51400},
    {"Orange", 0xFA6800}, {"Amber", 0xF0A30A}, {"Yellow", 0xE3C800}, {"Brown", 0x825A2C},
    {"Olive", 0x6D8764}, {"Steel", 0x647687}, {"Mauve", 0x76608A}, {"Taupe", 0x87794E},
};
#define N_ACCENTS ((int)(sizeof(s_accents) / sizeof(s_accents[0])))

/* ---------------------------------------------------------------- the settings */

#define ACCENT_AMBER 13                                 /* index in s_accents; the Froyo orange highlight is closest to it */
static int s_android = 1;                               /* the skin: the built-in shell is the Android style one; purr_shell_select() changes it when a loaded shell says otherwise */
#define SKIN_ANDROID (s_android)
static int s_type;                                      /* the Type look (the Playtime shell): paper, ink and the accent, square, outlined. A look over the W10M roles, not a third skin: every layout test in the shared screens keeps seeing W10M */

static int s_accent = ACCENT_AMBER;                     /* Amber in the Android skin, which is the default; Cobalt unless the flavor says otherwise in the Windows skin (purr_theme_set_skin) */
static int s_dark = 1;
static int s_transparency = 0;          /* solid by default: see-through sheets cost a blend per pixel */
static int s_tile_opa = 255;
static void (*s_on_change)(void);

/* ---------------------------------------------------------------- colours */

static lv_color_t c_accent(void) { return lv_color_hex(s_accents[s_accent].color); }
static lv_color_t c_sheet(void)   { if (s_type) return lv_color_hex(s_dark ? 0x14120F : 0xF4F1EA); return lv_color_hex(SKIN_ANDROID ? (s_dark ? 0x000000 : 0xFFFFFF) : (s_dark ? 0x1A1A1A : 0xF2F2F2)); }
static lv_color_t c_surface(void) { if (s_type) return lv_color_hex(s_dark ? 0x26231E : 0xE7E2D5); return lv_color_hex(SKIN_ANDROID ? (s_dark ? 0x1E1E1E : 0xE6E6E6) : (s_dark ? 0x2B2B2B : 0xDDDDDD)); }
static lv_color_t c_hi(void)      { if (s_type) return lv_color_hex(s_dark ? 0x3F3B33 : 0xCFC9BA); return lv_color_hex(SKIN_ANDROID ? (s_dark ? 0x3A3A3A : 0xBDBDBD) : (s_dark ? 0x404040 : 0xC4C4C4)); }
static lv_color_t c_text(void)    { if (s_type) return lv_color_hex(s_dark ? 0xF4F1EA : 0x14120F); return lv_color_hex(s_dark ? 0xFFFFFF : 0x000000); }
static lv_color_t c_dim(void)     { if (s_type) return lv_color_hex(s_dark ? 0xB5B0A3 : 0x6B675C); return lv_color_hex(SKIN_ANDROID ? (s_dark ? 0xAAAAAA : 0x555555) : (s_dark ? 0xB0B0B0 : 0x606060)); }
static lv_color_t c_row_pr(void)  { if (s_type) return lv_color_hex(s_accents[s_accent].color); return lv_color_hex(s_dark ? 0x333333 : 0xD0D0D0); }

/* Black or white, whichever reads on the accent (Lime and Yellow are light). In the Type look: ink or paper, the one with the higher contrast ratio (the same rule as
 * the shell's tp_on_accent(), which sdk/tests/test_type_shell.py checks for every colour in the picker). */
static float lum2(uint32_t c)
{
    float r = ((c >> 16) & 0xFF) / 255.0f, g = ((c >> 8) & 0xFF) / 255.0f, b = (c & 0xFF) / 255.0f;
    return 0.2126f * r * r + 0.7152f * g * g + 0.0722f * b * b;
}

static float contrast2(float x, float y)
{
    return x > y ? (x + 0.05f) / (y + 0.05f) : (y + 0.05f) / (x + 0.05f);
}

static lv_color_t c_on_accent(void)
{
    if (s_type) {
        float a = lum2(s_accents[s_accent].color);
        return contrast2(a, lum2(0x14120F)) >= contrast2(a, lum2(0xF4F1EA)) ? lv_color_hex(0x14120F) : lv_color_hex(0xF4F1EA);
    }
    uint32_t c = s_accents[s_accent].color;
    uint32_t lum = (299 * ((c >> 16) & 0xFF) + 587 * ((c >> 8) & 0xFF) + 114 * (c & 0xFF)) / 1000;
    return lum > 150 ? lv_color_black() : lv_color_white();
}

lv_color_t purr_theme_accent_color(void) { return c_accent(); }
/* Text that warns: amber on a dark sheet, a darker amber on paper (the Type look), where 0xFFB900 would not read. */
lv_color_t purr_theme_warn_color(void) { return lv_color_hex(s_type && !s_dark ? 0x9A5B00 : 0xFFB900); }
lv_color_t purr_theme_text_color(void) { return c_text(); }
lv_color_t purr_theme_dim_color(void) { return c_dim(); }
lv_color_t purr_theme_surface_color(void) { return c_surface(); }

int purr_theme_accent_count(void) { return N_ACCENTS; }
const purr_accent_t *purr_theme_accent_at(int i) { return (i >= 0 && i < N_ACCENTS) ? &s_accents[i] : NULL; }
int purr_theme_accent(void) { return s_accent; }
int purr_theme_dark(void) { return s_dark; }
int purr_theme_transparency(void) { return s_transparency; }
int purr_theme_tile_opacity(void) { return s_tile_opa; }

/* ---------------------------------------------------------------- the styles */

static lv_style_t s_st[PURR_TH_COUNT];
static lv_style_t s_tile_pr, s_accent_pr;
static lv_style_t s_row_div;                    /* Android skin: the divider under a list row */
/* standard widgets */
static lv_style_t s_btn, s_btn_pr;
static lv_style_t s_sw, s_sw_ind, s_sw_knob;
static lv_style_t s_sl, s_sl_ind, s_sl_knob;
static lv_style_t s_dd, s_dd_list, s_dd_sel;
static lv_style_t s_ta, s_ta_focus;
static lv_theme_t s_theme;
static int s_ready;

static void fill_styles_w10m(void)
{
    lv_color_t acc = c_accent();

    lv_style_set_bg_color(&s_st[PURR_TH_SHEET], c_sheet());
    lv_style_set_bg_opa(&s_st[PURR_TH_SHEET], s_transparency ? LV_OPA_90 : LV_OPA_COVER);

    lv_style_set_bg_color(&s_st[PURR_TH_STRIP], lv_color_black());
    lv_style_set_bg_opa(&s_st[PURR_TH_STRIP], s_transparency ? LV_OPA_50 : LV_OPA_COVER);

    lv_style_set_bg_color(&s_st[PURR_TH_TILE], acc);
    lv_style_set_bg_opa(&s_st[PURR_TH_TILE], (lv_opa_t)s_tile_opa);
    lv_style_set_bg_color(&s_tile_pr, lv_color_darken(acc, LV_OPA_20));
    lv_style_set_bg_opa(&s_tile_pr, (lv_opa_t)s_tile_opa);

    lv_style_set_bg_color(&s_st[PURR_TH_ACCENT_BG], acc);
    lv_style_set_bg_opa(&s_st[PURR_TH_ACCENT_BG], LV_OPA_COVER);
    lv_style_set_bg_color(&s_accent_pr, lv_color_darken(acc, LV_OPA_20));

    lv_style_set_text_color(&s_st[PURR_TH_ACCENT_TEXT], acc);

    lv_style_set_border_side(&s_st[PURR_TH_ACCENT_BORDER], LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_st[PURR_TH_ACCENT_BORDER], 3);
    lv_style_set_border_color(&s_st[PURR_TH_ACCENT_BORDER], acc);

    lv_style_set_text_color(&s_st[PURR_TH_TEXT], c_text());
    lv_style_set_text_color(&s_st[PURR_TH_DIM], c_dim());

    lv_style_set_bg_color(&s_st[PURR_TH_SURFACE], c_surface());
    lv_style_set_bg_opa(&s_st[PURR_TH_SURFACE], LV_OPA_COVER);

    lv_style_set_bg_color(&s_st[PURR_TH_ROW_PRESSED], c_row_pr());
    lv_style_set_bg_opa(&s_st[PURR_TH_ROW_PRESSED], LV_OPA_COVER);

    lv_style_set_image_recolor(&s_st[PURR_TH_ICON], acc);
    lv_style_set_image_recolor_opa(&s_st[PURR_TH_ICON], LV_OPA_COVER);

    /* button: flat, square, accent */
    lv_style_set_bg_color(&s_btn, acc);
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_text_color(&s_btn, c_on_accent());
    lv_style_set_radius(&s_btn, 0);
    lv_style_set_shadow_width(&s_btn, 0);
    lv_style_set_outline_width(&s_btn, 0);
    lv_style_set_border_width(&s_btn, 0);
    lv_style_set_bg_color(&s_btn_pr, lv_color_darken(acc, LV_OPA_20));

    /* switch: a square track, filled with the accent when on */
    lv_style_set_bg_color(&s_sw, c_hi());
    lv_style_set_bg_opa(&s_sw, LV_OPA_COVER);
    lv_style_set_radius(&s_sw, 0);
    lv_style_set_border_width(&s_sw, 0);
    lv_style_set_bg_color(&s_sw_ind, acc);
    lv_style_set_bg_opa(&s_sw_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_sw_ind, 0);
    lv_style_set_bg_color(&s_sw_knob, c_text());
    lv_style_set_bg_opa(&s_sw_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_sw_knob, 0);

    /* slider */
    lv_style_set_bg_color(&s_sl, c_hi());
    lv_style_set_bg_opa(&s_sl, LV_OPA_COVER);
    lv_style_set_radius(&s_sl, 0);
    lv_style_set_bg_color(&s_sl_ind, acc);
    lv_style_set_bg_opa(&s_sl_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_ind, 0);
    lv_style_set_bg_color(&s_sl_knob, acc);
    lv_style_set_bg_opa(&s_sl_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_knob, 0);

    /* dropdown and its list */
    lv_style_set_bg_color(&s_dd, c_surface());
    lv_style_set_bg_opa(&s_dd, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd, c_text());
    lv_style_set_radius(&s_dd, 0);
    lv_style_set_border_width(&s_dd, 2);
    lv_style_set_border_color(&s_dd, c_dim());
    lv_style_set_bg_color(&s_dd_list, c_surface());
    lv_style_set_bg_opa(&s_dd_list, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd_list, c_text());
    lv_style_set_radius(&s_dd_list, 0);
    lv_style_set_border_width(&s_dd_list, 2);
    lv_style_set_border_color(&s_dd_list, c_dim());
    lv_style_set_bg_color(&s_dd_sel, acc);
    lv_style_set_bg_opa(&s_dd_sel, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd_sel, c_on_accent());

    /* text area */
    lv_style_set_bg_color(&s_ta, c_surface());
    lv_style_set_bg_opa(&s_ta, LV_OPA_COVER);
    lv_style_set_text_color(&s_ta, c_text());
    lv_style_set_radius(&s_ta, 0);
    lv_style_set_border_width(&s_ta, 2);
    lv_style_set_border_color(&s_ta, c_dim());
    lv_style_set_border_color(&s_ta_focus, acc);
}

/* The Android 2.2 look: grey bevelled buttons with 5 px corners, black list rows with a grey divider, orange (the accent)
 * when pressed or selected, a gradient title bar and status strip. Gradients are two-stop and only on small objects; there
 * are no shadows and no translucency by default (UI/SHELL-OPTIMIZATION.md). Same roles as the W10M skin. */
static void fill_styles_android(void)
{
    lv_color_t acc = c_accent();
    lv_color_t acc_hi = lv_color_lighten(acc, LV_OPA_30);
    lv_color_t acc_lo = lv_color_darken(acc, LV_OPA_30);
    lv_color_t grey_hi = lv_color_hex(0xECECEC), grey_lo = lv_color_hex(0xB2B2B2), edge = lv_color_hex(0x555555);

    lv_style_set_bg_color(&s_st[PURR_TH_SHEET], c_sheet());
    lv_style_set_bg_opa(&s_st[PURR_TH_SHEET], s_transparency ? LV_OPA_90 : LV_OPA_COVER);

    lv_style_set_bg_color(&s_st[PURR_TH_STRIP], lv_color_hex(0x2E2E2E));
    lv_style_set_bg_grad_color(&s_st[PURR_TH_STRIP], lv_color_hex(0x0C0C0C));
    lv_style_set_bg_grad_dir(&s_st[PURR_TH_STRIP], LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_st[PURR_TH_STRIP], s_transparency ? LV_OPA_80 : LV_OPA_COVER);

    lv_style_set_bg_color(&s_st[PURR_TH_TILE], acc);                        /* the Android shell has no tiles; kept so the role is defined */
    lv_style_set_bg_opa(&s_st[PURR_TH_TILE], (lv_opa_t)s_tile_opa);
    lv_style_set_bg_color(&s_tile_pr, acc_lo);
    lv_style_set_bg_opa(&s_tile_pr, (lv_opa_t)s_tile_opa);

    lv_style_set_bg_color(&s_st[PURR_TH_ACCENT_BG], acc_hi);
    lv_style_set_bg_grad_color(&s_st[PURR_TH_ACCENT_BG], acc);
    lv_style_set_bg_grad_dir(&s_st[PURR_TH_ACCENT_BG], LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_st[PURR_TH_ACCENT_BG], LV_OPA_COVER);
    lv_style_set_bg_color(&s_accent_pr, acc_lo);
    lv_style_set_bg_grad_color(&s_accent_pr, acc_lo);

    lv_style_set_text_color(&s_st[PURR_TH_ACCENT_TEXT], acc);

    lv_style_set_border_side(&s_st[PURR_TH_ACCENT_BORDER], LV_BORDER_SIDE_FULL);
    lv_style_set_border_width(&s_st[PURR_TH_ACCENT_BORDER], 1);
    lv_style_set_border_color(&s_st[PURR_TH_ACCENT_BORDER], acc);

    lv_style_set_text_color(&s_st[PURR_TH_TEXT], c_text());
    lv_style_set_text_color(&s_st[PURR_TH_DIM], c_dim());

    lv_style_set_bg_color(&s_st[PURR_TH_SURFACE], c_surface());
    lv_style_set_bg_opa(&s_st[PURR_TH_SURFACE], LV_OPA_COVER);
    lv_style_set_border_width(&s_st[PURR_TH_SURFACE], 1);
    lv_style_set_border_color(&s_st[PURR_TH_SURFACE], c_hi());
    lv_style_set_radius(&s_st[PURR_TH_SURFACE], 4);

    lv_style_set_bg_color(&s_st[PURR_TH_ROW_PRESSED], acc_lo);              /* orange under the finger (pressed state only) */
    lv_style_set_bg_opa(&s_st[PURR_TH_ROW_PRESSED], LV_OPA_COVER);

    lv_style_set_border_side(&s_row_div, LV_BORDER_SIDE_BOTTOM);            /* the divider between list rows */
    lv_style_set_border_width(&s_row_div, 1);
    lv_style_set_border_color(&s_row_div, c_hi());

    lv_style_set_image_recolor(&s_st[PURR_TH_ICON], c_text());
    lv_style_set_image_recolor_opa(&s_st[PURR_TH_ICON], LV_OPA_COVER);

    lv_style_set_bg_color(&s_st[PURR_TH_TITLE_BAR], lv_color_hex(0x5B5B5B));
    lv_style_set_bg_grad_color(&s_st[PURR_TH_TITLE_BAR], lv_color_hex(0x2A2A2A));
    lv_style_set_bg_grad_dir(&s_st[PURR_TH_TITLE_BAR], LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_st[PURR_TH_TITLE_BAR], LV_OPA_COVER);
    lv_style_set_border_side(&s_st[PURR_TH_TITLE_BAR], LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_st[PURR_TH_TITLE_BAR], 1);
    lv_style_set_border_color(&s_st[PURR_TH_TITLE_BAR], lv_color_black());

    /* button: light grey bevel, dark text, orange when pressed */
    lv_style_set_bg_color(&s_btn, grey_hi);
    lv_style_set_bg_grad_color(&s_btn, grey_lo);
    lv_style_set_bg_grad_dir(&s_btn, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_text_color(&s_btn, lv_color_black());
    lv_style_set_radius(&s_btn, 5);
    lv_style_set_shadow_width(&s_btn, 0);
    lv_style_set_outline_width(&s_btn, 0);
    lv_style_set_border_width(&s_btn, 1);
    lv_style_set_border_color(&s_btn, edge);
    lv_style_set_bg_color(&s_btn_pr, acc_hi);
    lv_style_set_bg_grad_color(&s_btn_pr, acc);
    lv_style_set_border_color(&s_btn_pr, acc_lo);

    /* switch: a small rounded track, the accent when on, a grey knob */
    lv_style_set_bg_color(&s_sw, c_hi());
    lv_style_set_bg_opa(&s_sw, LV_OPA_COVER);
    lv_style_set_radius(&s_sw, 4);
    lv_style_set_border_width(&s_sw, 1);
    lv_style_set_border_color(&s_sw, edge);
    lv_style_set_bg_color(&s_sw_ind, acc);
    lv_style_set_bg_opa(&s_sw_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_sw_ind, 4);
    lv_style_set_bg_color(&s_sw_knob, grey_hi);
    lv_style_set_bg_grad_color(&s_sw_knob, grey_lo);
    lv_style_set_bg_grad_dir(&s_sw_knob, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_sw_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_sw_knob, 3);

    /* slider: a dark groove, the accent filled, a grey handle */
    lv_style_set_bg_color(&s_sl, c_hi());
    lv_style_set_bg_opa(&s_sl, LV_OPA_COVER);
    lv_style_set_radius(&s_sl, 3);
    lv_style_set_bg_color(&s_sl_ind, acc);
    lv_style_set_bg_opa(&s_sl_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_ind, 3);
    lv_style_set_bg_color(&s_sl_knob, grey_hi);
    lv_style_set_bg_grad_color(&s_sl_knob, grey_lo);
    lv_style_set_bg_grad_dir(&s_sl_knob, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_sl_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_knob, 5);
    lv_style_set_border_width(&s_sl_knob, 1);
    lv_style_set_border_color(&s_sl_knob, edge);

    /* dropdown (the Froyo spinner): the button look; its list is black with the orange selection */
    lv_style_set_bg_color(&s_dd, grey_hi);
    lv_style_set_bg_grad_color(&s_dd, grey_lo);
    lv_style_set_bg_grad_dir(&s_dd, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_dd, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd, lv_color_black());
    lv_style_set_radius(&s_dd, 5);
    lv_style_set_border_width(&s_dd, 1);
    lv_style_set_border_color(&s_dd, edge);
    lv_style_set_bg_color(&s_dd_list, c_surface());
    lv_style_set_bg_opa(&s_dd_list, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd_list, c_text());
    lv_style_set_radius(&s_dd_list, 4);
    lv_style_set_border_width(&s_dd_list, 1);
    lv_style_set_border_color(&s_dd_list, c_hi());
    lv_style_set_bg_color(&s_dd_sel, acc);
    lv_style_set_bg_opa(&s_dd_sel, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd_sel, c_on_accent());

    /* text area (the Froyo text field): a white box with dark text, an accent border when it has the keyboard */
    lv_style_set_bg_color(&s_ta, lv_color_white());
    lv_style_set_bg_opa(&s_ta, LV_OPA_COVER);
    lv_style_set_text_color(&s_ta, lv_color_black());
    lv_style_set_radius(&s_ta, 3);
    lv_style_set_border_width(&s_ta, 2);
    lv_style_set_border_color(&s_ta, lv_color_hex(0x808080));
    lv_style_set_border_color(&s_ta_focus, acc);
}

/* The Type look: the flat, square W10M roles in paper, ink and the accent (the colour functions above), with the outlines the Type shell draws everywhere: an ink
 * border on every control, the accent when pressed, an ink title bar with an accent rule. The highlight a d-pad moves is the shell's own ring (purr_tp_nav.c), so
 * no widget needs a focus style here. */
static void fill_styles_type(void)
{
    fill_styles_w10m();
    lv_color_t ink = lv_color_hex(s_dark ? 0xF4F1EA : 0x14120F);
    lv_color_t acc = c_accent();

    lv_style_set_bg_color(&s_st[PURR_TH_STRIP], lv_color_hex(0x14120F));

    lv_style_set_bg_color(&s_st[PURR_TH_TITLE_BAR], lv_color_hex(0x14120F));
    lv_style_set_bg_opa(&s_st[PURR_TH_TITLE_BAR], LV_OPA_COVER);
    lv_style_set_border_side(&s_st[PURR_TH_TITLE_BAR], LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_st[PURR_TH_TITLE_BAR], 2);
    lv_style_set_border_color(&s_st[PURR_TH_TITLE_BAR], acc);

    lv_style_set_border_side(&s_st[PURR_TH_SURFACE], LV_BORDER_SIDE_FULL);
    lv_style_set_border_width(&s_st[PURR_TH_SURFACE], 1);
    lv_style_set_border_color(&s_st[PURR_TH_SURFACE], ink);

    lv_style_set_border_width(&s_btn, 2);
    lv_style_set_border_color(&s_btn, lv_color_hex(0x14120F));

    lv_style_set_border_width(&s_sw, 2);
    lv_style_set_border_color(&s_sw, ink);
    lv_style_set_border_width(&s_sl_knob, 2);
    lv_style_set_border_color(&s_sl_knob, ink);

    lv_style_set_border_color(&s_dd, ink);
    lv_style_set_border_color(&s_dd_list, ink);
    lv_style_set_border_color(&s_ta, ink);
    lv_style_set_bg_color(&s_ta, s_dark ? c_surface() : lv_color_hex(0xFBF9F4));
}

static void fill_styles(void)
{
    if (SKIN_ANDROID) {
        fill_styles_android();
    } else if (s_type) {
        fill_styles_type();
    } else {
        fill_styles_w10m();
    }
}

void purr_theme_set_type_look(int on)
{
    s_type = on != 0;
    if (s_type) {
        s_dark = 0;                                     /* paper by default; a saved choice (load_file in purr_theme_init) overrides it */
    }
}

purr_skin_t purr_theme_skin(void) { return SKIN_ANDROID ? PURR_SKIN_ANDROID : PURR_SKIN_W10M; }

void purr_theme_set_skin(purr_skin_t skin)
{
    s_android = skin == PURR_SKIN_ANDROID;
    s_accent = s_android ? ACCENT_AMBER : PURR_BRAND_ACCENT_INDEX;             /* the default accent of the skin; a saved choice (load_file in purr_theme_init) overrides it */
}

void purr_theme_title_bar(lv_obj_t *head, lv_obj_t *arrow, lv_obj_t *title)
{
    purr_theme_apply(head, PURR_TH_TITLE_BAR);
    if (s_type) {
        lv_obj_set_style_text_color(title, lv_color_hex(0xF4F1EA), 0);      /* the bar is ink in both bases */
        lv_obj_set_style_text_color(arrow, lv_color_hex(0xF4F1EA), 0);
    }
    if (SKIN_ANDROID) {
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);     /* Back is a key; tapping the bar still goes back */
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);    /* the bar is dark grey in both the dark and the light base */
    }
}

lv_color_t purr_theme_button_text_color(void) { return SKIN_ANDROID ? lv_color_black() : (s_type ? c_on_accent() : lv_color_white()); }

void purr_theme_apply(lv_obj_t *obj, purr_th_style_t which)
{
    switch (which) {
    case PURR_TH_BUTTON:
        if (SKIN_ANDROID) {
            lv_obj_add_style(obj, &s_btn, 0);
            lv_obj_add_style(obj, &s_btn_pr, LV_STATE_PRESSED);
        } else {
            purr_theme_apply(obj, PURR_TH_ACCENT_BG);
        }
        return;
    case PURR_TH_LIST_ROW:
        if (SKIN_ANDROID) {
            purr_theme_apply(obj, PURR_TH_ROW_PRESSED);     /* the pressed fill and the divider */
        } else {
            purr_theme_apply(obj, PURR_TH_SURFACE);
            if (!s_type) {                                  /* the Type look outlines the row instead of an edge in the accent */
                purr_theme_apply(obj, PURR_TH_ACCENT_BORDER);
            }
            purr_theme_apply(obj, PURR_TH_ROW_PRESSED);
        }
        return;
    default:
        break;
    }
    lv_obj_add_style(obj, &s_st[which], 0);
    if (which == PURR_TH_TILE) {
        lv_obj_add_style(obj, &s_tile_pr, LV_STATE_PRESSED);
    } else if (which == PURR_TH_ACCENT_BG) {
        lv_obj_add_style(obj, &s_accent_pr, LV_STATE_PRESSED);
    } else if (which == PURR_TH_ROW_PRESSED) {
        /* the style is for the pressed state only */
        lv_obj_remove_style(obj, &s_st[which], 0);
        lv_obj_add_style(obj, &s_st[which], LV_STATE_PRESSED);
        if (SKIN_ANDROID) {
            lv_obj_add_style(obj, &s_row_div, 0);
        }
    }
}

/* The layer under every widget, the shell's and apps': standard widgets come out W10M. */
static void theme_apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_btn, 0);
        lv_obj_add_style(obj, &s_btn_pr, LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s_sw, 0);
        lv_obj_add_style(obj, &s_sw_ind, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s_sw_knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s_sl, 0);
        lv_obj_add_style(obj, &s_sl_ind, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_sl_knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_add_style(obj, &s_dd, 0);
    } else if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &s_dd_list, 0);
        lv_obj_add_style(obj, &s_dd_sel, LV_PART_SELECTED | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s_ta, 0);
        lv_obj_add_style(obj, &s_ta_focus, LV_STATE_FOCUSED);
    }
}

/* ---------------------------------------------------------------- saving */

#define SYS_FILE "/etc/theme.conf"

typedef struct {
    char *buf;
    uint32_t cap, len;
} rd_t;

static int rd_cb(void *ctx, const void *data, uint32_t len)
{
    rd_t *r = ctx;
    if (r->len + len > r->cap) {
        len = r->cap - r->len;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return 0;
}

static void parse(char *text)
{
    for (char *line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (strcmp(k, "accent") == 0) {
            for (int i = 0; i < N_ACCENTS; i++) {
                if (strcasecmp(v, s_accents[i].name) == 0) {
                    s_accent = i;
                }
            }
        } else if (strcmp(k, "base") == 0) {
            s_dark = strcmp(v, "light") != 0;
        } else if (strcmp(k, "transparency") == 0) {
            s_transparency = atoi(v) != 0;
        } else if (strcmp(k, "tile_opacity") == 0) {
            int n = atoi(v);
            s_tile_opa = n < 0 ? 0 : (n > 255 ? 255 : n);
        }
    }
}

static void load_file(const char *path)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs)) {
        return;
    }
    char buf[192];
    rd_t r = {buf, sizeof(buf) - 1, 0};
    if (purr_fs_read(fs, path, rd_cb, &r) < 0) {
        return;                         /* no file: the defaults stand */
    }
    buf[r.len] = '\0';
    parse(buf);
}

static char s_user[32];

static void write_file(const char *path, const char *text)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs)) {
        return;
    }
    for (size_t i = 1; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            char dir[96];
            if (i >= sizeof(dir)) {
                return;
            }
            memcpy(dir, path, i);
            dir[i] = '\0';
            purr_fs_mkdir(fs, dir);     /* an existing directory is just an ignored error */
        }
    }
    if (purr_fs_write(fs, path, text, (uint32_t)strlen(text)) < 0) {
        ESP_LOGW(TAG, "could not save %s", path);
    }
}

static void save(void)
{
    int64_t began = esp_timer_get_time();
    char text[128];
    snprintf(text, sizeof(text), "accent=%s\nbase=%s\ntransparency=%d\ntile_opacity=%d\n",
             s_accents[s_accent].name, s_dark ? "dark" : "light", s_transparency, s_tile_opa);
    write_file(SYS_FILE, text);         /* the sign-in screen reads this one */
    if (s_user[0] != '\0') {
        char path[80];
        snprintf(path, sizeof(path), "/home/%s/.config/theme.conf", s_user);
        write_file(path, text);
    }
    /* the flash is shared with every task: a slow write here can stall the app manager too */
    ESP_LOGI(TAG, "settings saved in %d ms", (int)((esp_timer_get_time() - began) / 1000));
}

/* ---------------------------------------------------------------- changing */

static void changed(void)
{
    fill_styles();
    lv_obj_report_style_change(NULL);   /* every object re-evaluates its styles */
    if (s_on_change != NULL) {
        s_on_change();
    }
}

void purr_theme_set_accent(int i)
{
    if (i >= 0 && i < N_ACCENTS && i != s_accent) {
        s_accent = i;
        changed();
        save();
    }
}

void purr_theme_set_dark(int dark)
{
    dark = dark != 0;
    if (dark != s_dark) {
        s_dark = dark;
        changed();
        save();
    }
}

void purr_theme_set_transparency(int on)
{
    on = on != 0;
    if (on != s_transparency) {
        s_transparency = on;
        changed();
        save();
    }
}

void purr_theme_set_tile_opacity(int opa)
{
    opa = opa < 0 ? 0 : (opa > 255 ? 255 : opa);
    if (opa != s_tile_opa) {
        s_tile_opa = opa;
        changed();
        save();
    }
}

void purr_theme_on_change(void (*cb)(void)) { s_on_change = cb; }

void purr_theme_load_user(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    snprintf(s_user, sizeof(s_user), "%s", name);
    char path[80];
    snprintf(path, sizeof(path), "/home/%s/.config/theme.conf", name);
    load_file(path);
    if (s_ready) {
        changed();
    }
}

/* ---------------------------------------------------------------- the same settings, by key
 *
 * `settings set theme.accent Red` from the shell, a script or the serial console does exactly what the
 * Settings app does. The setters touch LVGL, so each one hops onto the UI task. */

typedef struct {
    int what;
    char value[24];
    int rc;
    char err[64];
} th_job_t;

static void *th_set_cb(void *arg)
{
    th_job_t *j = arg;
    j->rc = 0;
    switch (j->what) {
    case 0: {                           /* accent, by name */
        int found = -1;
        for (int i = 0; i < N_ACCENTS; i++) {
            if (strcasecmp(j->value, s_accents[i].name) == 0) {
                found = i;
            }
        }
        if (found < 0) {
            snprintf(j->err, sizeof(j->err), "unknown colour");
            j->rc = -1;
        } else {
            purr_theme_set_accent(found);
        }
        break;
    }
    case 1:
        if (strcasecmp(j->value, "dark") == 0) purr_theme_set_dark(1);
        else if (strcasecmp(j->value, "light") == 0) purr_theme_set_dark(0);
        else { snprintf(j->err, sizeof(j->err), "expected dark or light"); j->rc = -1; }
        break;
    case 2: {
        int b;
        if (purr_settings_parse_bool(j->value, &b) != 0) { snprintf(j->err, sizeof(j->err), "expected on or off"); j->rc = -1; }
        else purr_theme_set_transparency(b);
        break;
    }
    case 3: {
        int n = atoi(j->value);
        if (n < 40 || n > 255) { snprintf(j->err, sizeof(j->err), "expected 40-255"); j->rc = -1; }
        else purr_theme_set_tile_opacity(n);
        break;
    }
    }
    return NULL;
}

static int th_set(int what, const char *v, char *err, size_t cap)
{
    th_job_t j = {.what = what};
    snprintf(j.value, sizeof(j.value), "%s", v);
    if (purr_ui_service_run(th_set_cb, &j, NULL) != ESP_OK) {
        snprintf(err, cap, "the screen is not running");
        return -1;
    }
    if (j.rc != 0) {
        snprintf(err, cap, "%s", j.err);
    }
    return j.rc;
}

static int accent_get(char *o, size_t c) { snprintf(o, c, "%s", s_accents[s_accent].name); return 0; }
static int accent_set(const char *v, char *e, size_t c) { return th_set(0, v, e, c); }
static int base_get(char *o, size_t c) { snprintf(o, c, "%s", s_dark ? "dark" : "light"); return 0; }
static int base_set(const char *v, char *e, size_t c) { return th_set(1, v, e, c); }
static int transp_get(char *o, size_t c) { snprintf(o, c, "%s", s_transparency ? "on" : "off"); return 0; }
static int transp_set(const char *v, char *e, size_t c) { return th_set(2, v, e, c); }
static int tileopa_get(char *o, size_t c) { snprintf(o, c, "%d", s_tile_opa); return 0; }
static int tileopa_set(const char *v, char *e, size_t c) { return th_set(3, v, e, c); }

static const purr_setting_t s_keys[] = {
    {"theme.accent", "accent colour (yours; saved per user)",
     "Lime Green Emerald Teal Cyan Cobalt Indigo Violet Pink Magenta Crimson Red Orange Amber Yellow Brown Olive Steel Mauve Taupe",
     0, accent_get, accent_set},
    {"theme.base", "light or dark background", "dark|light", 0, base_get, base_set},
    {"theme.transparency", "see-through status bar and sheets", "on|off", 0, transp_get, transp_set},
    {"theme.tile_opacity", "how solid the start tiles are", "40-255", 0, tileopa_get, tileopa_set},
};

void purr_theme_init(void)
{
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        purr_settings_register(&s_keys[i]);
    }
    for (int i = 0; i < PURR_TH_COUNT; i++) {
        lv_style_init(&s_st[i]);
    }
    lv_style_t *all[] = {&s_tile_pr, &s_accent_pr, &s_row_div, &s_btn, &s_btn_pr, &s_sw, &s_sw_ind, &s_sw_knob,
                         &s_sl, &s_sl_ind, &s_sl_knob, &s_dd, &s_dd_list, &s_dd_sel, &s_ta, &s_ta_focus};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        lv_style_init(all[i]);
    }
    load_file(SYS_FILE);
    fill_styles();

    lv_theme_t *base = lv_display_get_theme(NULL);
    lv_theme_set_apply_cb(&s_theme, theme_apply_cb);
    if (base != NULL) {
        lv_theme_set_parent(&s_theme, base);
    }
    lv_display_set_theme(NULL, &s_theme);
    s_ready = 1;
    ESP_LOGI(TAG, "theme: accent %s, %s, transparency %s, tiles %d", s_accents[s_accent].name,
             s_dark ? "dark" : "light", s_transparency ? "on" : "off", s_tile_opa);
}
