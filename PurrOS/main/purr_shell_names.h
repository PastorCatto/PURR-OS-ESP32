/*
 * purr_shell_names.h - the built-in (Android style) shell's share of the shared shell interface gets its own names.
 *
 * Every shell implements the same 18 functions that the shared code (Settings, the Developer Portal, the task manager, the permission prompt, the ui catcall
 * windows, the notify log) calls: the interface of purr_w10m_priv.h plus purr_shell_notified(). A loaded shell hands them over in its ops table
 * (purr_shell_abi.h); the built-in shell's files define PURR_SHELL_IMPL_ANDROID and include this header before anything else, so inside them the names become
 * `<name>_droid` (calls from them to the interface stay direct), and purr_shell.c defines the real names as wrappers that call the shell that is running.
 * A new function in the interface needs a line below, a field in purr_shell_ops_t and a wrapper in purr_shell.c.
 */
#ifndef PURR_SHELL_NAMES_H
#define PURR_SHELL_NAMES_H

#if defined(PURR_SHELL_IMPL_ANDROID)
#define w10m_apply_orientation w10m_apply_orientation_droid
#define w10m_apps_changed w10m_apps_changed_droid
#define w10m_apps_changed_post w10m_apps_changed_post_droid
#define w10m_banner w10m_banner_droid
#define w10m_banner_post w10m_banner_post_droid
#define w10m_busy_begin w10m_busy_begin_droid
#define w10m_busy_end w10m_busy_end_droid
#define w10m_dev_badge_update w10m_dev_badge_update_droid
#define w10m_disp_h w10m_disp_h_droid
#define w10m_disp_w w10m_disp_w_droid
#define w10m_get_brightness w10m_get_brightness_droid
#define w10m_img w10m_img_droid
#define w10m_is_locked w10m_is_locked_droid
#define w10m_note_post w10m_note_post_droid
#define w10m_place_sheet w10m_place_sheet_droid
#define w10m_save_display w10m_save_display_droid
#define w10m_set_brightness w10m_set_brightness_droid
#define purr_shell_notified purr_shell_notified_droid
#endif

#endif /* PURR_SHELL_NAMES_H */
