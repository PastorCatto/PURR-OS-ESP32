/*
 * purr_tp_shim.c - what the d-pad navigation (purr_tp_nav.c) asks of a shell, for the images that have none (plan7.md, "The d-pad for every sheet").
 *
 * KittenOS (the recovery app), the internet loader and the setup wizard run without the Type shell, and some of them without the Settings, portal and window
 * code at all. Every function here is weak: where a shell defines the real one (the Type shell's purr_tp.c, the sheets' own files) its definition wins at link
 * time, and where there is none this is the answer for "nothing of that kind is open".
 */
#include "purr_tp_priv.h"

#include "purr_catrt_ui.h"

__attribute__((weak)) int tp_signed_in(void) { return 1; }
__attribute__((weak)) int tp_modal_up(void) { return 0; }
__attribute__((weak)) void tp_pad_hint(int mask) { (void)mask; }
__attribute__((weak)) int w10m_perm_is_open(void) { return 0; }
__attribute__((weak)) lv_obj_t *w10m_settings_root(void) { return NULL; }
__attribute__((weak)) lv_obj_t *w10m_devportal_root(void) { return NULL; }
__attribute__((weak)) lv_obj_t *w10m_tasks_root(void) { return NULL; }
__attribute__((weak)) lv_obj_t *purr_catrt_ui_front(void) { return NULL; }
__attribute__((weak)) int w10m_disp_w(void) { return lv_display_get_horizontal_resolution(NULL); }
__attribute__((weak)) int w10m_disp_h(void) { return lv_display_get_vertical_resolution(NULL); }
