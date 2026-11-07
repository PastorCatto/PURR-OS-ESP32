/*
 * purr_w10m_module.c - the Windows 10 Mobile style shell as a loadable module (plan7.md, "Shells as installable items").
 *
 * Built by `purrstrap shells build` together with purr_w10m.c (the same source the kernel compiles when the shell is built in); NOT part of the kernel's
 * source list. The loader calls purr_shell_module_main() once and gets the ops table the kernel's shell dispatch (purr_shell.c) forwards to.
 */
#include "purr_shell_abi.h"

#include "purr_notify.h"
#include "purr_w10m.h"
#include "purr_w10m_priv.h"

static const purr_shell_ops_t s_ops = {
    .abi_version = PURR_SHELL_ABI_VERSION,
    .skin = PURR_SHELL_SKIN_W10M,
    .id = "w10m",
    .start = purr_w10m_start,
    .set_orientation = purr_w10m_set_orientation,
    .lock = purr_w10m_lock,
    .notify = purr_w10m_notify,
    .refresh_apps = purr_w10m_refresh_apps,
    .apply_orientation = w10m_apply_orientation,
    .apps_changed = w10m_apps_changed,
    .apps_changed_post = w10m_apps_changed_post,
    .banner = w10m_banner,
    .banner_post = w10m_banner_post,
    .busy_begin = w10m_busy_begin,
    .busy_end = w10m_busy_end,
    .dev_badge_update = w10m_dev_badge_update,
    .disp_h = w10m_disp_h,
    .disp_w = w10m_disp_w,
    .get_brightness = w10m_get_brightness,
    .img = w10m_img,
    .is_locked = w10m_is_locked,
    .note_post = w10m_note_post,
    .place_sheet = w10m_place_sheet,
    .save_display = w10m_save_display,
    .set_brightness = w10m_set_brightness,
    .shell_notified = purr_shell_notified,
};

const purr_shell_ops_t *purr_shell_module_main(void)
{
    return &s_ops;
}
