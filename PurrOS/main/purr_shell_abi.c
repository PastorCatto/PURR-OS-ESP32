/*
 * purr_shell_abi.c - the kernel's side of the shell ABI: the real address of every name in purr_shell_exports.def (purr_shell_abi.h).
 *
 * The table is built from the same list the module build reads, so a name that does not exist (or was renamed) in the kernel is a compile error here, not a
 * crash on a device. The headers below are the ones the Windows shell includes: they declare everything the list names.
 */
#include "purr_brand.h"
#include "esp_attr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "commands.h"
#include "login.h"
#include "purr_appmgr.h"
#include "purr_battery.h"
#include "purr_catrt.h"
#include "purr_catrt_game.h"
#include "purr_catrt_ui.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "purr_settings.h"
#include "purr_ui_config.h"
#include "purr_lvgl.h"
#include "purr_ui_assets.h"
#include "purr_power_plat.h"
#include "purr_ui_service.h"
#include "purr_notify.h"
#include "purr_w10m_priv.h"
#include "purr_wallpaper.h"
#include "purr_resetnote_os.h"
#include "purr_sysupdate.h"
#include "purr_w10m_theme.h"

#include "purr_shell_abi.h"

#define SHELL_FN(n) (const void *)n,
#define SHELL_DATA(t, n) (const void *)&n,
const void *const purr_shell_exports[] = {
#include "purr_shell_exports.def"
};
#undef SHELL_FN
#undef SHELL_DATA

const size_t purr_shell_export_count = sizeof(purr_shell_exports) / sizeof(purr_shell_exports[0]);
