/*
 * purr_shell_headers.h - every header a loadable shell is compiled against that the export list (purr_shell_exports.def) and the layout list
 * (purr_shell_layout.def) need to be able to name things: the kernel's table (purr_shell_abi.c) and a shell's entry file include this one, so the two see the same declarations.
 * It is the include list of the Windows 10 Mobile shell.
 */
#ifndef PURR_SHELL_HEADERS_H
#define PURR_SHELL_HEADERS_H

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
#include "purr_shell.h"
#include "purr_w10m_priv.h"
#include "purr_wallpaper.h"
#include "purr_resetnote_os.h"
#include "purr_sysupdate.h"
#include "purr_w10m_theme.h"

#endif /* PURR_SHELL_HEADERS_H */
